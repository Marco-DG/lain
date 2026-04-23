# Chapter 13 — Memory Model

## 13.1 Overview

Lain's memory model is designed around three principles:

1. **Deterministic allocation** — All allocation is explicit and predictable.
2. **Zero runtime overhead** — No garbage collector, no reference counting.
3. **C99 compatibility** — Memory layout follows C99 rules exactly.

## 13.2 Stack Allocation [Implemented]

### 13.2.1 Default Allocation

All local variables, structs, and arrays are stack-allocated. There is no
implicit heap allocation:

```lain
var x int = 42               // stack: 4 bytes
var arr int[100]              // stack: 400 bytes
var p Point                   // stack: sizeof(Point) bytes
```

### 13.2.2 Function Frames

Each function call creates a stack frame containing:
- All local variables
- All parameters (passed by value or pointer, depending on type)
- Temporary values for expression evaluation

Stack frames are automatically reclaimed when the function returns.

### 13.2.3 No Implicit Heap

The compiler never implicitly allocates on the heap. Every heap allocation
is explicit through C interop (`malloc`/`free`) or custom allocators.

## 13.3 Heap Allocation [Implemented]

### 13.3.1 Via C Interop

Heap allocation is performed through extern declarations of C's memory
management functions:

```lain
c_include "<stdlib.h>"

extern proc malloc(size usize) mov *void
extern proc free(ptr mov *void)

proc main() {
    var ptr = malloc(1024)    // heap allocation — caller owns
    // ... use ptr ...
    free(mov ptr)             // explicit deallocation
}
```

### 13.3.2 Ownership Integration

Heap-allocated resources are tracked by the ownership system:

- `malloc` returns `mov *void` — the caller receives ownership.
- `free` takes `mov *void` — ownership is consumed.
- Forgetting to `free` a `mov` pointer is a compile error (`[E003]`).
- Double-free is prevented by double-consume detection (`[E002]`) and
  use-after-move detection (`[E001]`).

This turns memory management bugs into compile-time errors without any
runtime overhead.

## 13.4 No Garbage Collector [Implemented]

Lain has no garbage collector:

- **No GC pauses** — Execution is never interrupted for memory management.
- **No reference counting** — No runtime overhead for tracking references.
- **No tracing** — No graph traversal or reachability analysis at runtime.
- **Deterministic deallocation** — Resources are freed at known, predictable
  program points.

This makes Lain suitable for real-time, embedded, and performance-critical
applications where GC pauses are unacceptable.

## 13.5 Value Semantics [Implemented]

### 13.5.1 Copy vs Move

| Type Category | Assignment `b = a` | Function Argument |
|:--------------|:-------------------|:------------------|
| Primitives (`int`, `bool`, etc.) | Bitwise copy | Pass by value |
| Non-linear structs | Field-by-field copy | Pass by `const T*` |
| Linear structs (`mov` fields) | Copy forbidden | Requires `mov` or borrow |
| Non-linear arrays | Element-by-element copy | Pass by `const T*` |
| Linear arrays | Copy forbidden | Requires `mov` |
| Slices | Copy `{data, len}` pair | Pass by value |
| Raw pointers | Copy address | Pass by value |

### 13.5.2 Parameter Passing Convention

The C99 backend passes composite types by pointer for efficiency:

| Lain Parameter | C99 Equivalent |
|:---------------|:---------------|
| `f(x int)` | `void f(int32_t x)` |
| `f(x Point)` | `void f(const main_Point* x)` |
| `f(var x Point)` | `void f(main_Point* x)` |
| `f(mov x Point)` | `void f(main_Point x)` |

Shared parameters for structs become `const T*` in C. Mutable parameters
become `T*`. Owned parameters are passed by value (the original is
invalidated by move semantics, so no aliasing is possible).

## 13.6 Alignment and Layout [Implemented]

### 13.6.1 C99 Layout Rules

Lain inherits C99's alignment and padding rules:

- Structs are laid out in declaration order.
- Each field is aligned to its natural alignment.
- The compiler inserts padding between fields as needed.
- Struct size is rounded up to a multiple of the largest field alignment.

### 13.6.2 No Layout Control

Lain currently provides no mechanism for controlling struct layout:

- No `#pragma pack` equivalent.
- No alignment annotations.
- No `repr(C)` or `repr(packed)`.

The layout is always the default C layout, which ensures interoperability
with C libraries but prevents bitwise-exact layouts for protocols or hardware
registers.

### 13.6.3 ADT Layout

ADTs are laid out as tagged unions:

```c
typedef struct {
    enum { Tag_Variant1, Tag_Variant2 } tag;
    union {
        struct { /* Variant1 fields */ } Variant1;
        struct { /* Variant2 fields */ } Variant2;
    };
} TypeName;
```

The tag field occupies `sizeof(enum)` bytes (typically 4). The union is
sized to the largest variant.

## 13.7 Array Memory [Implemented]

### 13.7.1 Fixed-Size Arrays

Fixed-size arrays are allocated inline (on the stack for locals, within
the struct for fields):

```lain
var arr int[10]              // 40 bytes on stack (contiguous)
```

### 13.7.2 Array Initialization

Arrays can be initialized with:
- **Array literals**: `var arr = [1, 2, 3]`
- **Default zero**: `var arr int[10]` (zero-initialized)
- **`undefined`**: `var arr int[10] = undefined` (uninitialized)

### 13.7.3 Slices

Slices are fat pointers containing a data pointer and length:

```lain
var s u8[:0]                 // { data: *u8, len: isize }
```

The slice itself is stack-allocated (two words). The data it points to
may be anywhere: stack, heap, or static storage.

## 13.8 Static Storage [Implemented]

### 13.8.1 String Literals

String literals are stored in static (read-only) storage and have type
`u8[:0]` (null-terminated byte slice):

```lain
var msg = "hello"            // msg.data points to static storage
```

### 13.8.2 Global Variables

Global variables are stored in static storage:

```lain
var counter int = 0          // static storage duration
```

## 13.9 Undefined Behavior

The following operations produce undefined behavior at runtime:

| Operation | Cause |
|:----------|:------|
| Null pointer dereference | Dereferencing `*0` in `unsafe` block |
| Buffer overflow | Pointer arithmetic past allocation bounds in `unsafe` |
| Use of uninitialized memory | Reading `undefined` values |
| Integer overflow | Wrapping arithmetic on signed types |

> Note: Lain compiles with `-fwrapv`, making signed integer overflow defined
> as two's complement wrapping. This removes one source of UB compared to
> standard C99.

---

*This chapter is normative.*
