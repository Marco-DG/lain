# Chapter 11 — C Interoperability

## 11.1 Overview

Lain compiles to C99 and provides direct interoperability with C libraries
through three mechanisms:

1. **`c_include`** — Injects C `#include` directives into generated code.
2. **`extern` declarations** — Declares C functions and types for use in Lain.
3. **Type mapping** — Maps Lain types to their C equivalents.

This interop is zero-overhead: extern declarations produce direct C function
calls with no wrapper or marshaling layer.

## 11.2 The `c_include` Declaration [Implemented]

### 11.2.1 Syntax

```
c_include_decl = "c_include" STRING_LITERAL ;
```

### 11.2.2 Semantics

`c_include` causes the compiler to emit a `#include` directive in the
generated C code:

```lain
c_include "<stdio.h>"      // emits: #include <stdio.h>
c_include "<stdlib.h>"     // emits: #include <stdlib.h>
c_include "mylib.h"        // emits: #include "mylib.h"
```

The string content is emitted verbatim. If the string begins with `<`, the
angle-bracket form is used; otherwise, the quoted form is used.

### 11.2.3 Placement

`c_include` declarations appear at module top level, typically at the
beginning of a file before any other declarations.

> **CONSTRAINT:** `c_include` shall only appear at the top level of a module.

## 11.3 Extern Function Declarations [Implemented]

### 11.3.1 Syntax

```
extern_func = "extern" "func" IDENTIFIER "(" param_list ")" [return_type] ;
extern_proc = "extern" "proc" IDENTIFIER "(" param_list ")" [return_type] ;
```

### 11.3.2 Extern Functions (`extern func`)

An `extern func` declares a C function that Lain treats as pure — it may
be called from `func` contexts:

```lain
extern func strlen(s *u8) int
```

> **CONSTRAINT:** An `extern func` may be called from both `func` and `proc`
> contexts. The programmer asserts that the C function has no observable side
> effects.

### 11.3.3 Extern Procedures (`extern proc`)

An `extern proc` declares a C function with potential side effects:

```lain
extern proc printf(fmt *u8, ...) int
extern proc fopen(filename *u8, mode *u8) mov *FILE
extern proc fclose(stream mov *FILE) int
```

> **CONSTRAINT:** An `extern proc` may only be called from `proc` contexts.
> Calling an `extern proc` from a `func` produces diagnostic `[E011]`.

### 11.3.4 Ownership Annotations on Extern Parameters

Extern declarations use the same ownership annotations as regular functions:

| Annotation | Meaning |
|:-----------|:--------|
| `param *T` | Shared pointer (const in C) |
| `var param *T` | Mutable pointer (non-const in C) |
| `mov param *T` | Ownership transfer (caller loses access) |

```lain
extern proc fopen(filename *u8, mode *u8) mov *FILE
// filename and mode are shared (read-only)
// return value transfers ownership to caller

extern proc fclose(stream mov *FILE) int
// stream ownership is consumed by fclose
```

These annotations integrate with the borrow checker — the compiler tracks
ownership of extern resources just like native Lain values.

### 11.3.5 Variadic Parameters

Extern declarations may use `...` for C variadic functions:

```lain
extern proc printf(fmt *u8, ...) int
```

> **CONSTRAINT:** Variadic parameters (`...`) shall only appear in `extern`
> declarations. User-defined variadic functions are not supported.

### 11.3.6 No Body

Extern declarations have no body. They are terminated by a newline or
semicolon:

```lain
extern func abs(x int) int     // no body — implemented in C
```

## 11.4 Extern Type Declarations [Implemented]

### 11.4.1 Syntax

```
extern_type = "extern" "type" IDENTIFIER ;
```

### 11.4.2 Semantics

An `extern type` declares an opaque C type. The compiler emits a
`typedef struct` in the generated C code:

```lain
extern type FILE
```

Generated C:
```c
typedef struct FILE FILE;
```

The type can then be used through pointers:

```lain
extern proc fopen(filename *u8, mode *u8) mov *FILE
var f = fopen("data.txt", "r")
// f has type *FILE — opaque, can only be passed to extern functions
```

> **CONSTRAINT:** Extern types are opaque. Their fields cannot be accessed,
> their size is unknown, and they can only be used through pointers.

## 11.5 Type Mapping [Implemented]

### 11.5.1 Primitive Type Mapping

| Lain Type | C Type |
|:----------|:-------|
| `int` | `int32_t` |
| `i8` | `int8_t` |
| `i16` | `int16_t` |
| `i32` | `int32_t` |
| `i64` | `int64_t` |
| `u8` | `uint8_t` |
| `u16` | `uint16_t` |
| `u32` | `uint32_t` |
| `u64` | `uint64_t` |
| `f32` | `float` |
| `f64` | `double` |
| `bool` | `bool` (from `<stdbool.h>`) |
| `isize` | `intptr_t` |
| `usize` | `uintptr_t` |

### 11.5.2 Pointer Mapping

| Lain Type | C Type |
|:----------|:-------|
| `*T` (shared) | `const T*` |
| `var *T` (mutable) | `T*` |
| `mov *T` (owned) | `T*` |

### 11.5.3 String Interop

Lain's string type `u8[:0]` (null-terminated byte slice) has a `.data` field
that yields a `*u8`, which maps to C's `const uint8_t*`. For C functions
expecting `const char*`, special-cased emit logic maps `*u8` to `const char*`
for known standard library functions.

> **Known limitation:** The `u8` vs `char` type mismatch prevents general
> string interop with arbitrary C libraries. A `char` type or annotation
> system is under consideration (see TASKS.md Q5).

### 11.5.4 Struct Mapping

Lain structs are emitted as C structs with the same field layout:

```lain
type Point { x int, y int }
```

Generated C:
```c
typedef struct { int32_t x; int32_t y; } main_Point;
```

### 11.5.5 Enum Mapping

Lain enums are emitted as C enums:

```lain
type Color { Red, Green, Blue }
```

Generated C:
```c
typedef enum { main_Color_Red, main_Color_Green, main_Color_Blue } main_Color;
```

### 11.5.6 ADT Mapping

Lain ADTs (enums with data) are emitted as tagged unions:

```lain
type Shape {
    Circle { radius int }
    Rectangle { width int, height int }
}
```

Generated C:
```c
typedef struct {
    enum { main_Shape_Circle, main_Shape_Rectangle } tag;
    union {
        struct { int32_t radius; } Circle;
        struct { int32_t width; int32_t height; } Rectangle;
    };
} main_Shape;
```

## 11.6 Name Mangling [Implemented]

### 11.6.1 Convention

All Lain declarations are prefixed with the module path in the generated C
code, using underscores as separators:

```
module_path + "_" + declaration_name
```

For example:
- `func add` in `std/math.ln` → `std_math_add`
- `type File` in `std/fs.ln` → `std_fs_File`
- `proc main` in `main.ln` → `main` (special case: the entry point is not mangled)

### 11.6.2 Extern Name Preservation

Extern declarations preserve their original names in the generated C code.
The `c_name` for an extern is the declared name without module prefix,
allowing direct linkage with C libraries.

## 11.7 Lain Runtime Header [Implemented]

The compiler emits a small header at the top of every generated C file:

```c
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
```

This provides the fixed-width integer types, `bool`, and `size_t` that
Lain's type system depends on.

## 11.8 Limitations and Known Issues

### 11.8.1 No Callback Support

Lain does not currently support function pointer types. C functions that
take callbacks cannot be wrapped.

### 11.8.2 No Struct Layout Control

Lain does not provide `#pragma pack` or alignment annotations. Struct layout
follows the C compiler's default alignment rules.

### 11.8.3 char vs u8

The most significant interop limitation is the `char` vs `uint8_t` mismatch.
Lain uses `u8` for bytes (emitting `uint8_t`), but C string functions expect
`char*`. The current workaround is special-case emit logic for known functions.

---

*This chapter is normative for implemented features.*
