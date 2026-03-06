# Chapter 2 — Type System

## 2.1 Overview

Lain is a statically typed language. Every variable, expression, and function
parameter has a type determined at compile time. Types are never inspected at
runtime — there is no runtime type information (RTTI).

The type system comprises:

- Primitive types (integers, floats, booleans)
- Composite types (structs, enums, ADTs, arrays, slices, pointers)
- Opaque types (extern C types)
- Special types (void, type-as-value)
- User-defined type aliases

Every type additionally carries an **ownership mode** (see §7) that determines
how values of that type may be passed and consumed.

## 2.2 Primitive Types [Implemented]

### 2.2.1 Integer Types

| Type | Size | Range | C Equivalent |
|:-----|:-----|:------|:-------------|
| `int` | Platform-dependent (>= 32-bit) | Signed | `int` |
| `i8` | 8-bit | -128 to 127 | `int8_t` |
| `i16` | 16-bit | -32,768 to 32,767 | `int16_t` |
| `i32` | 32-bit | -2^31 to 2^31-1 | `int32_t` |
| `i64` | 64-bit | -2^63 to 2^63-1 | `int64_t` |
| `u8` | 8-bit | 0 to 255 | `uint8_t` |
| `u16` | 16-bit | 0 to 65,535 | `uint16_t` |
| `u32` | 32-bit | 0 to 2^32-1 | `uint32_t` |
| `u64` | 64-bit | 0 to 2^64-1 | `uint64_t` |
| `isize` | Pointer-sized signed | Platform-dependent | `ptrdiff_t` |
| `usize` | Pointer-sized unsigned | Platform-dependent | `size_t` |

`int` is the default integer type. Its exact size is platform-dependent but
shall be at least 32 bits. It corresponds to C's `int`.

`isize` and `usize` have the same size as a pointer on the target platform.
They are used for array indices, sizes, and pointer arithmetic.

### 2.2.2 Floating-Point Types

| Type | Size | Precision | C Equivalent |
|:-----|:-----|:----------|:-------------|
| `f32` | 32-bit | ~7 decimal digits | `float` |
| `f64` | 64-bit | ~16 decimal digits | `double` |

Floating-point types conform to IEEE 754. `f64` is the default floating-point
type for literals containing a decimal point.

### 2.2.3 Boolean Type

`bool` — a type with exactly two values: `true` and `false`. Stored as a
single byte.

> **CONSTRAINT:** There are no implicit conversions between `bool` and any
> integer type. `if 1 { ... }` is valid only because integer literals in
> condition position are implicitly compared to zero. Explicit conversion
> requires the `as` operator or comparison.

### 2.2.4 The `void` Type

`void` represents the absence of a value. It is used exclusively in pointer
types (`*void`) for C interoperability, analogous to C's `void*`.

> **CONSTRAINT:** Variables shall not be declared with type `void`.
> `var x void` is a compile error.

`void` may appear in:
- Pointer types: `*void`, `var *void`, `mov *void`
- Return types of procedures that return no value (implicit)

## 2.3 Implicit Conversions — Forbidden [Implemented]

Lain strictly forbids all implicit type conversions. There is:
- No implicit widening (`u8` to `int`)
- No implicit truncation (`int` to `u8`)
- No implicit bool-to-int or int-to-bool conversion
- No implicit float-to-int or int-to-float conversion
- No implicit pointer coercion

Every conversion between types shall use the explicit `as` operator (see §4.8):

```lain
var x i32 = 1000
var y = x as u8           // explicit truncation: 1000 -> 232
var big = 42 as i64       // explicit widening
```

> **CONSTRAINT:** Passing a value of type `T1` where type `T2` is expected
> shall produce a type error if `T1 != T2`, even if `T1` is a subrange of `T2`.

## 2.4 Structs [Implemented]

A struct groups named fields into a single value type:

```lain
type Point {
    x int,
    y int
}

type Color {
    r u8, g u8, b u8, a u8
}
```

### 2.4.1 Struct Declaration

```
struct_decl = "type" IDENTIFIER "{" field_list "}" ;
field_list  = field { "," field } ;
field       = ["mov"] IDENTIFIER type_expr ;
```

Fields are separated by commas. The last comma before `}` is optional.

### 2.4.2 Struct Construction

Structs are constructed positionally — all fields must be provided in
declaration order:

```lain
var p = Point(10, 20)
var c = Color(255, 128, 0, 255)
```

> **CONSTRAINT:** The number of arguments in a struct constructor shall
> equal the number of fields. Missing or extra arguments are compile errors.

### 2.4.3 Field Access

Fields are accessed via dot notation:

```lain
var px = p.x              // read field
p.x = 42                  // write field (only if p is mutable)
```

> **CONSTRAINT:** Writing to a field requires the variable to be declared
> with `var` (mutable). Writing to an immutable variable's field is a
> compile error.

### 2.4.4 Value Semantics

Non-linear structs (those without `mov` fields) have value semantics —
assignment copies all fields:

```lain
var p2 = p                // copies all fields
p2.x = 99                 // does not affect p
```

### 2.4.5 Linear Struct Fields

Fields annotated with `mov` make the containing struct **linear** — the struct
must be consumed exactly once and cannot be copied (see §7.6):

```lain
type File {
    mov handle *FILE       // owned resource
}
// File is linear: must be explicitly consumed
```

If any field of a struct is `mov`, the entire struct is linear. This property
is transitive — a struct containing a linear struct is itself linear.

## 2.5 Enums (Simple Enumerations) [Implemented]

Simple enumerations define a type with a fixed set of named values, without
associated data:

```lain
type Color { Red, Green, Blue }
type Direction { North, South, East, West }
```

### 2.5.1 Enum Declaration

```
enum_decl = "type" IDENTIFIER "{" variant_list "}" ;
variant_list = IDENTIFIER { "," IDENTIFIER } ;
```

### 2.5.2 Enum Values

Enum values are accessed via the type name:

```lain
var c = Color.Red
var d = Direction.North
```

### 2.5.3 Enum Representation

Enums are represented as integers in the generated C code. Variants are
numbered starting from 0 in declaration order.

## 2.6 Algebraic Data Types (ADTs) [Implemented]

ADTs extend enums with associated data per variant. A type declaration is
an ADT if at least one variant has fields:

```lain
type Shape {
    Circle     { radius int }
    Rectangle  { width int, height int }
    Point                                  // no associated data
}
```

### 2.6.1 ADT Declaration

```
adt_decl     = "type" IDENTIFIER "{" adt_variants "}" ;
adt_variants = adt_variant { "," adt_variant } ;
adt_variant  = IDENTIFIER [ "{" field_list "}" ] ;
```

A type is classified as:
- **Struct** — if it has no variant names, only fields (e.g., `type Point { x int, y int }`)
- **Enum** — if all variants have no fields (e.g., `type Color { Red, Green, Blue }`)
- **ADT** — if at least one variant has fields (e.g., `type Shape { Circle { r int }, Point }`)

### 2.6.2 ADT Construction

```lain
var s = Shape.Circle(10)
var r = Shape.Rectangle(5, 3)
var p = Shape.Point
```

Variants with fields are constructed with positional arguments, like structs.
Variants without fields are referenced by name alone (no parentheses).

### 2.6.3 ADT Destructuring

ADTs are destructured via pattern matching with `case` (see §5.7):

```lain
case s {
    Circle(r):       libc_printf("radius: %d\n", r)
    Rectangle(w, h): libc_printf("%d x %d\n", w, h)
    Point:           libc_printf("point\n")
}
```

### 2.6.4 ADT Representation

ADTs are represented in C as a tagged union: an integer tag field
indicating the active variant, plus a union of variant payloads.

## 2.7 Arrays (Fixed-Size) [Implemented]

Arrays have a compile-time-known size:

```lain
var arr int[5]                    // 5 ints, requires initialization
var arr2 = int[3]{1, 2, 3}       // initialized with literal values
```

### 2.7.1 Array Declaration

```
array_type = type_expr "[" integer_literal "]" ;
```

The size shall be a compile-time constant integer greater than zero.

### 2.7.2 Array Properties

| Property | Type | Description |
|:---------|:-----|:------------|
| `.len` | `int` (compile-time constant) | Number of elements |

### 2.7.3 Array Indexing

```lain
var x = arr[i]            // read element at index i
arr[i] = 42               // write element (if arr is mutable)
```

> **CONSTRAINT:** Array index expressions are statically verified via
> Value Range Analysis (see §8). The compiler shall prove that
> `0 <= i < arr.len` for every array access. Accesses that cannot be
> statically proven safe are rejected as compile errors.

### 2.7.4 Array Initialization

Arrays may be initialized with array literal syntax:

```lain
var arr = int[3]{1, 2, 3}
```

> **CONSTRAINT:** The number of elements in an array literal shall equal
> the declared array length.

Arrays may also be initialized with `undefined` (see §3.5):

```lain
var buffer u8[4096] = undefined   // all elements uninitialized
```

### 2.7.5 Array Value Semantics

Arrays with non-linear element types have value semantics — assignment
copies all elements. Arrays with linear element types are themselves linear.

## 2.8 Slices [Implemented]

Slices are dynamically-sized views into contiguous memory:

| Syntax | Description |
|:-------|:------------|
| `T[]` | Slice of T |
| `T[:S]` | Sentinel-terminated slice of T (terminated by value S) |

### 2.8.1 Slice Representation

A slice is a fat pointer consisting of:

```
struct { data: *T, len: usize }
```

`.data` is a pointer to the first element. `.len` is the number of elements.

### 2.8.2 Null-Terminated Slices

`T[:0]` denotes a slice terminated by a zero sentinel. The `.len` property
returns the number of elements **before** the sentinel.

String literals have type `u8[:0]` — a null-terminated byte slice:

```lain
var s = "hello"           // Type: u8[:0], .len = 5
var ptr = s.data          // Type: *u8
```

### 2.8.3 Slice Properties

| Property | Type | Description |
|:---------|:-----|:------------|
| `.len` | `int` | Number of elements (excluding sentinel) |
| `.data` | `*T` | Pointer to first element |

## 2.9 Pointers [Implemented]

Pointers represent memory addresses. They come in three ownership modes:

| Syntax | Mode | C Equivalent | Tracking |
|:-------|:-----|:-------------|:---------|
| `*T` | Shared (read-only) | `const T*` | Not tracked by borrow checker |
| `var *T` | Mutable | `T*` | Not tracked by borrow checker |
| `mov *T` | Owned | `T*` | Tracked as linear (must consume) |

### 2.9.1 Pointer Operations

| Operation | Syntax | Requirement |
|:----------|:-------|:------------|
| Dereference | `*ptr` | Requires `unsafe` block |
| Address-of | `&x` | Requires `unsafe` block |

Both dereferencing and taking the address of a variable are restricted to
`unsafe` blocks (see §12).

### 2.9.2 Null Pointers

Lain does not have a `null` keyword. Integer literal `0` may be used to
represent a null pointer when interfacing with C. Safe Lain code should
not need null pointers — use `Option(T)` instead (see §14).

## 2.10 Opaque Types (Extern) [Implemented]

Types defined in C whose layout is unknown to Lain:

```lain
extern type FILE
extern type sqlite3
```

Opaque types can only be used behind pointers (`*FILE`, `mov *FILE`). Their
size, alignment, and field layout are unknown to the Lain compiler.

> **CONSTRAINT:** Opaque types shall not be used as value types — only as
> pointer targets. `var f FILE` is a compile error.

## 2.11 Nested Types (Namespaces) [Implemented]

Types can be nested within other types using dot notation to create
logical namespaces:

```lain
type Token {
    // ...
}

type Token.Kind {
    Identifier,
    Number,
    String
}
```

Nested types are accessed using dot notation: `Token.Kind.Identifier`.

Nested types create a naming hierarchy but do not imply any containment
or ownership relationship between the outer and inner types.

## 2.12 Type Aliases [Implemented]

Type aliases create a new name for an existing type or for a compile-time
evaluated type expression:

```lain
type OptInt = Option(int)
type IntResult = Result(int, int)
```

Type aliases are semantically transparent — the alias and its target are
the same type for all purposes.

## 2.13 Type Equivalence [Implemented]

Lain uses **nominal type equivalence**: two types are the same if and only
if they have the same name. Structural similarity is not sufficient:

```lain
type Point { x int, y int }
type Vec2  { x int, y int }

// Point and Vec2 are DIFFERENT types, despite identical fields
```

Two values of different types cannot be assigned to each other, even if
their layouts are identical.

### 2.13.1 Structural Equality

The `==` and `!=` operators are supported for:
- Primitive numeric types (`int`, `i8`-`i64`, `u8`-`u64`, `f32`, `f64`)
- `bool`
- Pointers
- Enums (compared by variant tag)

> **CONSTRAINT:** Using `==` or `!=` on structs, arrays, or ADTs is a
> compile error. Field-by-field comparison must be performed manually.

## 2.14 Type Syntax Summary

```
type_expr = simple_type
          | pointer_type
          | array_type
          | slice_type
          | comptime_type ;

simple_type   = IDENTIFIER { "." IDENTIFIER } ;
pointer_type  = "*" type_expr ;
array_type    = type_expr "[" integer_literal "]" ;
slice_type    = type_expr "[" "]"
              | type_expr "[" ":" expr "]" ;
comptime_type = "comptime" type_expr ;
```

Ownership modes are not part of the type syntax per se — they are applied
contextually through `var`, `mov`, and default (shared) annotations on
parameters and declarations. See §7.1 for details.

## 2.15 Type-as-Value [Implemented]

In compile-time contexts (see §9), types can be used as first-class values
of the meta-type `type`:

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}
```

The `type` keyword in parameter position denotes the meta-type. A value of
meta-type `type` can be used in struct/enum definitions and type annotations
within the body of a `comptime` function.

---

*This chapter is normative. All type rules stated herein apply to every
conforming Lain implementation.*
