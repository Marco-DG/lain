# Chapter 3 — Declarations & Scoping

## 3.1 Declaration Kinds [Implemented]

Lain programs consist of two levels of declarations:

- **Top-level declarations**: type definitions, function/procedure declarations,
  global variables, imports, extern declarations, `c_include` directives
- **Local declarations**: variable bindings within function/procedure bodies

## 3.2 Variable Declarations [Implemented]

### 3.2.1 Mutable Bindings (`var`)

The `var` keyword creates a mutable binding — a variable that can be reassigned:

```lain
var x = 42                // mutable, type inferred as int
var y int = 0             // mutable, type explicit
var buffer u8[1024] = undefined  // mutable, explicitly uninitialized
```

Mutable bindings may be reassigned:

```lain
var x = 10
x = 20                    // OK: x is mutable
x += 5                    // OK: compound assignment
```

### 3.2.2 Immutable Bindings

An assignment without `var` to a name not already in scope creates an
immutable binding:

```lain
x = 42                    // immutable binding (x not previously in scope)
// x = 99                 // ERROR: cannot assign to immutable variable
```

### 3.2.3 Binding vs Assignment Disambiguation

Since `x = 10` can represent either a new binding or an assignment to an
existing variable, the compiler applies the following rule:

1. If `x` is **not** in scope: `x = 10` creates a new **immutable** binding.
2. If `x` **is** in scope and was declared `var`: `x = 10` is an **assignment**.
3. If `x` **is** in scope and is immutable: `x = 10` is a **compile error**.

```lain
x = 10                    // new immutable binding
var y = 20                // new mutable binding
y = 30                    // assignment to mutable y
// x = 40                 // ERROR: x is immutable
```

> **CONSTRAINT:** Assigning to an immutable variable shall produce
> diagnostic `[E009]`.

### 3.2.4 `var` as Mutable Mode Annotation

In parameter declarations and at call sites, `var` serves as a mutable
borrow mode annotation rather than a variable declaration keyword:

```lain
func mutate(var data Data) { ... }  // var = mutable borrow
mutate(var my_data)                  // var at call site = explicit mutable borrow
```

See §7.2 for the full ownership mode system.

## 3.3 Type Inference [Implemented]

When the type annotation is omitted, the type is inferred from the initializer:

```lain
var x = 42              // int
var s = "hello"         // u8[:0]
var b = true            // bool
var p = Point(1, 2)     // Point
var c = Color.Red       // Color
```

> **CONSTRAINT:** A variable declaration without both a type annotation and
> an initializer is ill-formed. At least one must be present.

## 3.4 Explicit Type Annotations [Implemented]

Types can be explicitly specified after the variable name:

```lain
var x int = 42
var y u8 = 255
var arr int[10] = undefined
```

When both a type annotation and an initializer are present, the type of
the initializer shall be compatible with the declared type.

## 3.5 The `undefined` Keyword [Implemented]

The `undefined` keyword explicitly marks a variable as uninitialized:

```lain
var x int = undefined          // x contains indeterminate data
var p = Point(10, undefined)   // p.y contains indeterminate data
var buffer u8[4096] = undefined // entire array is uninitialized
```

### 3.5.1 Definite Initialization Analysis

The compiler performs flow-sensitive definite initialization analysis on
variables declared with `undefined`:

> **CONSTRAINT:** Reading a variable declared with `= undefined` before
> it has been assigned a value on all code paths shall produce a compile error.

```lain
var x int = undefined
if condition {
    x = 42
} else {
    x = 0
}
// OK: x is definitely initialized on all paths
use(x)

var y int = undefined
if condition {
    y = 42
}
// ERROR: y may not be initialized (no else branch)
use(y)
```

### 3.5.2 Restrictions on `undefined`

> **CONSTRAINT:** Immutable bindings shall not use `undefined`.
> `x = undefined` is a compile error.

> **CONSTRAINT:** `undefined` shall only appear in:
> - Variable initializers: `var x = undefined`
> - Struct constructor arguments: `Point(10, undefined)`
> - Array initializers: `var arr int[N] = undefined`

## 3.6 Global Variables [Implemented]

Variables can be declared at module (file) scope:

```lain
var global_counter int = 0    // mutable global variable
```

Global variables persist for the entire program lifetime.

> **CONSTRAINT:** Pure functions (`func`) shall not read or write global
> mutable variables. Only procedures (`proc`) may access global mutable
> state. See §6.1 for the purity model.

## 3.7 Lexical Block Scoping [Implemented]

Variables are scoped to the block in which they are declared. A block is
delimited by `{` and `}`.

When a block exits, all local variables declared within it go out of scope.

```lain
proc example() {
    var x = 10
    if true {
        var y = 20        // y is only visible here
    }
    // y is not accessible here
}
```

### 3.7.1 Scope Nesting

Inner blocks can access variables from enclosing blocks:

```lain
var x = 10
if true {
    var y = x + 5         // OK: x is visible from enclosing scope
}
```

### 3.7.2 Shadowing

A variable declaration in an inner scope may shadow a variable with the
same name in an enclosing scope:

```lain
var x = 10
if true {
    var x = 20            // shadows outer x
    // x is 20 here
}
// x is 10 here (outer x restored)
```

Shadowing creates a new, independent variable — it does not modify the
original. The outer variable becomes visible again when the inner scope exits.

Shadowing is also allowed within the same scope:

```lain
var x = 42
var x = "hello"           // shadows previous x in the same scope
```

## 3.8 Top-Level Declarations [Implemented]

### 3.8.1 Type Declarations

See §2.4 (structs), §2.5 (enums), §2.6 (ADTs), §2.12 (type aliases).

### 3.8.2 Function and Procedure Declarations

See §6.

### 3.8.3 Import Declarations

See §10.

### 3.8.4 Extern Declarations

See §11.

### 3.8.5 C Include Declarations

See §11.1.

## 3.9 Forward References [Implemented]

Lain uses a multi-pass compiler. Functions, procedures, and types can be
referenced before they are declared in the source file:

```lain
func main() int { return helper() }
func helper() int { return 42 }     // OK: defined after use
```

No forward declarations or header files are needed.

> **CONSTRAINT:** Circular type definitions (a type containing itself by
> value) are ill-formed. Circular references through pointers are allowed.

## 3.10 Declaration Order [Implemented]

Within a single source file, top-level declarations may appear in any order.
There is no requirement that types be declared before functions that use them,
or that functions be declared before their callers.

Within a function body, statements are executed in order. A local variable
shall be declared before it is used:

```lain
// ERROR: y is not yet declared
var x = y + 1
var y = 42
```

> **CONSTRAINT:** Using a local variable before its declaration point
> shall produce diagnostic `[E013]` (undeclared variable).

---

*This chapter is normative.*
