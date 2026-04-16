# Chapter 0 — Overview, Scope & Conformance

## 0.1 Scope

This document specifies the Lain programming language. It defines the syntax,
semantics, and constraints that every conforming Lain implementation shall
enforce. It serves as the single authoritative reference for both language
users and implementors.

Lain is a **statically typed, compiled programming language** designed for
systems programming, embedded systems, and safety-critical software. It
achieves memory safety, type safety, and resource safety entirely at compile
time, with zero runtime overhead.

This specification covers:

- Lexical structure and grammar
- Type system and type inference
- Variable declarations, scoping, and initialization
- Expressions, operators, and evaluation order
- Statements and control flow
- Functions, procedures, and purity enforcement
- Ownership, borrowing, and linear type system
- Type constraints and static verification (Value Range Analysis)
- Compile-time evaluation and generics
- Pattern matching and exhaustiveness checking
- Module system
- C interoperability
- Unsafe code boundaries
- Memory model
- Error handling conventions
- Standard library
- Diagnostic messages

This specification does not cover:

- The internal architecture of any particular compiler implementation
- Build system integration or tooling
- IDE or editor support
- Operating system interfaces beyond what the standard library provides

## 0.2 The Five Pillars

Lain's design is governed by five non-negotiable goals, listed in strict
priority order. When design decisions conflict, higher-priority pillars
take precedence.

| Priority | Pillar | Description |
|----------|--------|-------------|
| 1 | **Assembly-Speed Performance** | Zero runtime overhead. No garbage collector, no reference counting, no hidden allocations. Generated code shall be as fast as hand-written C. |
| 2 | **Zero-Cost Memory Safety** | Guaranteed absence of use-after-free, double-free, data races, buffer overflows, and memory leaks — all enforced at compile time with no runtime cost. |
| 3 | **Static Verification** | Compile-time proof of program properties: bounds safety via Value Range Analysis, constraint satisfaction, exhaustive pattern matching. All analyses are decidable and polynomial-time. No SMT solver. |
| 4 | **Determinism** | Pure functions (`func`) are guaranteed to terminate and produce the same output for the same input. Side effects are confined to procedures (`proc`). |
| 5 | **Syntactic Simplicity** | Clean, readable syntax with no hidden magic. Explicit ownership annotations at call sites. No implicit conversions, no hidden copies. |

## 0.3 Guiding Principles

The following principles inform every language design decision:

**Soundness over completeness.** The compiler may reject valid programs
(false positives) but shall never accept invalid programs (false negatives).
A rejected program that is actually safe is an inconvenience; an accepted
program that is actually unsafe is a defect.

**Explicitness at boundaries.** Ownership transfer (`mov`) and mutable
access (`var`) are explicit at every call site. A reader of a function call
shall always know what happens to each argument without consulting the
function signature.

**No hidden costs.** Every operation's cost is visible in the source code.
No implicit copies, no hidden heap allocations, no virtual dispatch, no
implicit conversions.

**Composition over inheritance.** Lain has no classes, no inheritance, no
vtables. Behavior is composed through functions, Universal Function Call
Syntax (UFCS), and algebraic data types.

**C as the escape hatch.** When Lain's safety system is too restrictive,
`unsafe` blocks and C interoperability provide a controlled exit. The
safety boundary is always explicit and auditable.

## 0.4 Target Domains

Lain is designed for:

- **Embedded systems** — bare-metal firmware, RTOS tasks, device drivers
- **Systems programming** — operating system components, compilers, interpreters
- **Safety-critical software** — aerospace, medical, automotive
- **Performance-critical applications** — game engines, real-time audio, network stacks

## 0.5 Non-Goals

Lain explicitly does **not** aim to:

- Support garbage collection (ever)
- Support runtime reflection or dynamic dispatch
- Be a general-purpose scripting language
- Support exceptions or stack unwinding
- Provide implicit memory management of any kind
- Provide a runtime type information (RTTI) system

## 0.6 Conformance

### 0.6.1 Conforming Implementation

A conforming Lain implementation shall:

1. Accept all programs that satisfy the rules in this specification
2. Reject all programs that violate a constraint marked with `CONSTRAINT:`
3. Produce a diagnostic message for every rejected program
4. Compile accepted programs to executable code that, when run, produces
   behavior consistent with the semantic rules described herein
5. Implement all features marked `[Implemented]` in each chapter

A conforming implementation may:

1. Provide additional warnings beyond those required by this specification
2. Accept programs through implementation-defined extensions, provided
   such extensions are documented and do not alter the semantics of
   conforming programs

### 0.6.2 Conforming Program

A conforming Lain program:

1. Uses only the syntax and semantics defined in this specification
2. Does not depend on undefined behavior
3. Does not depend on implementation-defined extensions

### 0.6.3 Implementation Limits

A conforming implementation shall support at least the following limits:

| Resource | Minimum |
|----------|---------|
| Identifier length | 255 characters |
| Nesting depth (blocks) | 64 levels |
| Function parameters | 127 parameters |
| Array elements | 2^31 - 1 |
| Module import depth | 32 levels |
| `case` arms | 256 arms |
| Struct fields | 256 fields |
| ADT variants | 256 variants |

## 0.7 Compilation Model

Lain uses a **source-to-C transpilation** model:

```
.ln source files --> [Lain Compiler] --> out.c --> [C99 Compiler] --> executable
```

The Lain compiler performs all safety checks (ownership, borrowing, bounds,
exhaustiveness, purity) during compilation. The generated C99 code contains
no runtime safety checks — all guarantees are enforced statically.

### 0.7.1 Compiler Passes

A conforming implementation shall perform the following analyses in
a defined order:

| Pass | Purpose |
|------|---------|
| 1. Lexical analysis | Source text to token stream |
| 2. Parsing | Token stream to abstract syntax tree |
| 3. Name resolution | Resolve identifiers, build scope chains |
| 4. Type checking | Infer and verify types, check constraints |
| 5. Exhaustiveness checking | Verify pattern matching completeness |
| 6. Use analysis (NLL) | Compute last-use points for each variable |
| 7. Linearity checking | Verify ownership, borrowing, and move semantics |
| 8. Code emission | Generate C99 source code |

Passes 3-7 collectively form the **semantic analysis** phase. A program
that passes all seven analysis passes without error is a **well-formed
program** and shall be emitted as valid C99 code.

## 0.8 Document Conventions

### 0.8.1 Normative Language

This specification uses the following terms with precise meaning:

| Term | Meaning |
|------|---------|
| **shall** | An absolute requirement. Violation is non-conforming. |
| **shall not** | An absolute prohibition. |
| **may** | A permitted but not required behavior. |
| **should** | A recommended practice. Non-compliance is conforming but discouraged. |

### 0.8.2 Code Examples

Code examples use the following conventions:

```lain
// Valid code — this shall compile and execute correctly
var x = 42

// INVALID — this shall produce a compile error
// var y = *ptr    // ERROR: dereference outside unsafe
```

Lines prefixed with `// ERROR:` indicate code that a conforming
implementation shall reject.

### 0.8.3 Implementation Status

Each section header includes a status marker:

- **[Implemented]** — The current reference implementation enforces this rule.
- **[Planned]** — Specified here as part of the language design, but not yet
  implemented. A conforming implementation is not required to implement
  planned features until they are promoted to Implemented.
- **[Extension]** — A future direction under consideration. Not normative.

### 0.8.4 Constraint Markers

Rules that impose compile-time constraints are marked:

> **CONSTRAINT:** Description of the constraint.

Violation of a constraint shall produce a diagnostic message and reject
the program.

### 0.8.5 Undefined Behavior

Situations where the language cannot statically prevent incorrect behavior
are marked:

> **UNDEFINED BEHAVIOR:** Description and rationale.

A conforming program shall not depend on undefined behavior.

## 0.9 Safety Guarantees

The following table summarizes the safety properties that Lain guarantees
for all conforming programs (excluding `unsafe` blocks):

| Safety Concern | Guarantee | Mechanism | Chapter |
|:---------------|:----------|:----------|:--------|
| Buffer overflows | Impossible | Static Value Range Analysis | §8 |
| Use-after-free | Impossible | Linear types — exactly-once consumption | §7 |
| Double free | Impossible | Linear types — compile-time uniqueness | §7 |
| Data races | Impossible | Borrow checker — exclusive mutability | §7 |
| Null dereference | Prevented | Safe references cannot be null; raw ptrs require `unsafe` | §12 |
| Memory leaks | Prevented | Linear variables must be consumed | §7 |
| Division by zero | Preventable | Type constraints (`b int != 0`) | §8 |
| Integer overflow | Documented | Two's complement wrapping (`-fwrapv`) | §2 |
| Uninitialized memory | Prevented | Definite Initialization Analysis | §3 |
| Purity violations | Impossible | `func` cannot call `proc` or access globals | §6 |

## 0.10 Notation

### 0.10.1 Grammar Notation

Grammar rules in this specification use Extended Backus-Naur Form (EBNF):

```
rule        = definition ;
"keyword"   = terminal (literal text)
IDENTIFIER  = terminal (token class)
[ x ]       = optional (zero or one)
{ x }       = repetition (zero or more)
x | y       = alternation
( x y )     = grouping
```

The complete formal grammar is in Chapter 17.

### 0.10.2 Type Notation

Types are written in Lain syntax:

- `int`, `u8`, `bool` — primitive types
- `*T` — shared pointer to T
- `var *T` — mutable pointer to T
- `mov *T` — owned pointer to T
- `T[N]` — fixed-size array of N elements of type T
- `T[]` — slice of T
- `T[:S]` — sentinel-terminated slice of T

### 0.10.3 Cross-References

Internal cross-references use the format `(see §X.Y)` where X is the
chapter number and Y is the section number.

---

*This chapter is normative. All conformance requirements stated herein
apply to every conforming Lain implementation.*
