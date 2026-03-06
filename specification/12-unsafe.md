# Chapter 12 — Unsafe Code

## 12.1 Overview

Lain's safety guarantees (ownership, borrow checking, static bounds) apply
to all code by default. The `unsafe` block provides an escape hatch for
operations that the compiler cannot statically verify — primarily raw pointer
manipulation.

Unsafe code is explicitly delimited, auditable, and cannot accidentally
spread into safe code.

## 12.2 The Unsafe Block [Implemented]

### 12.2.1 Syntax

```
unsafe_stmt = "unsafe" "{" { statement } "}" ;
```

### 12.2.2 Semantics

An `unsafe` block marks a region of code where certain normally-prohibited
operations are allowed:

```lain
proc example() {
    var p *int = 0

    unsafe {
        var val int = *p     // pointer dereference — allowed
    }

    // var val2 int = *p     // ERROR: dereference outside unsafe
}
```

The unsafe block is a statement — it can appear anywhere a statement is valid.

## 12.3 What Unsafe Allows [Implemented]

### 12.3.1 Raw Pointer Dereference

The primary operation that requires `unsafe` is dereferencing a raw pointer:

```lain
unsafe {
    var value int = *ptr     // read through pointer
    *ptr = 42                // write through pointer (if mutable)
}
```

> **CONSTRAINT:** Dereferencing a raw pointer (`*ptr`) outside an `unsafe`
> block shall produce a diagnostic error.

### 12.3.2 Direct ADT Field Access

Accessing fields of an ADT variant directly (bypassing pattern matching)
requires `unsafe`:

```lain
type Shape {
    Circle { radius int }
    Rectangle { width int, height int }
}

var s = Shape.Circle(5)

unsafe {
    var r = s.radius         // direct field access — no tag check
}
```

> **CONSTRAINT:** Direct field access on an ADT value outside `case` pattern
> matching and outside an `unsafe` block shall produce a diagnostic error.
> This is unsafe because the compiler cannot verify that the value holds
> the expected variant.

## 12.4 What Unsafe Does NOT Allow

Unsafe blocks do not disable other safety checks:

| Safety Feature | Disabled by `unsafe`? |
|:---------------|:---------------------:|
| Pointer dereference | Yes |
| Direct ADT field access | Yes |
| Ownership tracking | **No** |
| Borrow checking | **No** |
| Move semantics | **No** |
| Type checking | **No** |
| Bounds checking | **No** |
| Division by zero detection | **No** |
| Exhaustiveness checking | **No** |

The ownership and borrowing system remains fully active inside `unsafe`
blocks. Use-after-move, double-move, and borrow conflicts are still
compile errors even within `unsafe`.

## 12.5 Nesting [Implemented]

Unsafe blocks may be nested. The inner block inherits the unsafe context
from the outer block:

```lain
unsafe {
    unsafe {
        var val int = *p     // OK — nested unsafe
    }
    var val2 int = *p        // OK — outer unsafe still active
}
```

The compiler tracks unsafe context with a boolean flag that is saved and
restored at block boundaries, supporting arbitrary nesting.

## 12.6 Unsafe in Control Flow [Implemented]

Unsafe blocks can appear inside control flow structures, and control flow
can appear inside unsafe blocks:

```lain
// Unsafe inside control flow
if condition {
    unsafe {
        var val int = *p     // OK
    }
}

// Control flow inside unsafe
unsafe {
    if condition {
        var val int = *p     // OK — inherits unsafe context
    }
}
```

## 12.7 Null Pointers [Implemented]

Lain allows creating null pointers by assigning `0` to a pointer variable:

```lain
var p *int = 0               // null pointer
```

Dereferencing a null pointer is undefined behavior. The compiler does not
currently perform null pointer analysis.

> **UNDEFINED BEHAVIOR:** Dereferencing a null pointer inside an `unsafe`
> block produces undefined behavior at runtime. The compiler does not prevent
> or detect this.

## 12.8 Address-Of Operator [Implemented]

The `&` operator obtains the address of a variable, producing a pointer:

```lain
var x int = 42
unsafe {
    var p *int = &x
    var val int = *p         // val == 42
}
```

> Note: The `&` operator for taking addresses is available within unsafe
> contexts. In safe code, `&` is used for non-consuming match syntax
> (`case &expr`).

## 12.9 Design Rationale

### 12.9.1 Why Unsafe Exists

Systems programming requires low-level memory operations that cannot be
statically verified: hardware register access, custom allocators, FFI
callbacks, and performance-critical data structure internals.

### 12.9.2 Why Unsafe Is Minimal

Lain's `unsafe` is deliberately narrow. Unlike C where all code is
implicitly unsafe, or Rust where `unsafe` unlocks multiple capabilities,
Lain's `unsafe` currently only enables pointer dereference and direct ADT
field access. This minimizes the audit surface.

### 12.9.3 Auditability

Because unsafe blocks are syntactically explicit and narrowly scoped,
auditing a Lain codebase for memory safety requires examining only the
`unsafe { }` blocks. All other code is guaranteed safe by the compiler.

---

*This chapter is normative.*
