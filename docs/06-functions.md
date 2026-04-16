# Chapter 6 — Functions & Procedures

## 6.1 The `func` / `proc` Distinction [Implemented]

Lain enforces a strict separation between **pure functions** and
**side-effecting procedures** at the type level:

| Property | `func` | `proc` |
|:---------|:-------|:-------|
| Side effects | Forbidden | Allowed |
| `for` loops | Allowed | Allowed |
| Unbounded `while` | Forbidden | Allowed |
| Bounded `while` (`decreasing`) | Allowed | Allowed |
| Recursion | Forbidden | Allowed |
| Calling `proc` | Forbidden | Allowed |
| Calling `func` | Allowed | Allowed |
| Calling `extern proc` | Forbidden | Allowed |
| Calling `extern func` | Allowed | Allowed |
| Global mutable state | No access | Full access |
| Termination | Guaranteed | Not guaranteed |

### 6.1.1 Pure Functions (`func`)

A `func` is a mathematical function: same inputs always produce the same
output, and evaluation always terminates.

```lain
func add(a int, b int) int {
    return a + b
}
```

**Termination guarantee**: `func` is guaranteed to terminate because:
1. Unbounded `while` loops are forbidden — no unbounded iteration.
2. Recursion (direct or indirect) is forbidden — no stack overflow.
3. Only `for` loops over finite ranges are allowed.
4. Bounded `while` loops (with a termination measure) are allowed — the
   compiler statically verifies that the measure is non-negative and
   strictly decreasing (see §5.6.2.1).

> **CONSTRAINT:** A `func` shall not:
> - Call any `proc` or `extern proc` (diagnostic `[E011]`)
> - Use unbounded `while` loops without `decreasing` (diagnostic `[E011]`)
> - Call itself or any function that (transitively) calls it
> - Read or write global mutable variables (diagnostic `[E011]`)

### 6.1.2 Procedures (`proc`)

A `proc` is the general-purpose callable unit. It has no restrictions on
side effects, loops, recursion, or global state:

```lain
proc log(msg u8[:0]) {
    libc_printf("%s\n", msg.data)
}
```

Procedures may call both `func` and `proc`. A procedure's termination
is not guaranteed by the compiler.

## 6.2 Function Declaration [Implemented]

### 6.2.1 Syntax

```
func_decl = ("func" | "fun") IDENTIFIER "(" param_list ")" [return_type] block ;
proc_decl = "proc" IDENTIFIER "(" param_list ")" [return_type] block ;

param_list = [ param { "," param } ] ;
param      = ["var" | "mov"] IDENTIFIER type_expr [constraints] ;

return_type = type_expr [constraints] ;
```

`fun` is an alias for `func`.

### 6.2.2 Parameters

Parameters declare their name, type, and ownership mode:

```lain
func read_only(x Data) int { ... }           // shared borrow (default)
func mutate(var x Data) { ... }              // mutable borrow
func consume(mov x Data) { ... }             // ownership transfer
func destruct(mov {a, b} Pair) int { ... }   // destructured ownership
```

See §7.2 for the three ownership modes.

### 6.2.3 Destructured Parameters

Parameters can destructure a struct, extracting named fields:

```lain
func drop(mov {handle} File) {
    fclose(handle)
}
```

The syntax `mov {field1, field2} Type` extracts the named fields from the
struct value, consuming it in the process.

### 6.2.4 Variadic Parameters

Variadic parameters (`...`) are only allowed in `extern` declarations:

```lain
extern proc printf(fmt *u8, ...) int
```

> **CONSTRAINT:** `...` shall only appear in `extern` declarations.
> User-defined variadic functions are not supported.

## 6.3 Return Types [Implemented]

### 6.3.1 Return by Value (Default)

```lain
func compute(a int, b int) int {
    return a + b
}
```

### 6.3.2 Return with Ownership (`mov`)

```lain
func make_file() mov File {
    return File(fopen("f.txt", "r"))
}
```

The return type `mov T` indicates the caller receives ownership of the
returned value.

### 6.3.3 Return by Mutable Reference (`var`)

```lain
func get_ref(var ctx Context) var int {
    return var ctx.counter
}
```

The return type `var T` creates a **persistent borrow** — the caller's
use of the returned reference keeps the borrow of the source alive.

> **CONSTRAINT:** `return var` shall only return references to data that
> outlives the function. Returning a `var` reference to a local variable
> produces diagnostic `[E010]` (dangling reference).

### 6.3.4 Void Return

Procedures may omit the return type to return void:

```lain
proc do_work() {
    // no return value
}
```

### 6.3.5 Return Type Constraints

Return types may carry constraints verified by the compiler (see §8):

```lain
func abs(x int) int >= 0 {
    if x < 0 { return 0 - x }
    return x
}
```

> **CONSTRAINT:** All return paths shall satisfy the declared return
> constraints. Violating a return constraint produces a diagnostic.

## 6.4 Caller-Site Annotations [Implemented]

When calling a function, the caller shall explicitly annotate ownership
transfers:

```lain
read_data(data)           // shared borrow (no annotation)
modify(var data)          // mutable borrow (var annotation)
consume(mov data)         // ownership transfer (mov annotation)
```

> **CONSTRAINT:** The caller-site annotation shall match the parameter mode:
> - Shared parameter: no annotation required
> - `var` parameter: caller shall write `var arg`
> - `mov` parameter: caller shall write `mov arg`
>
> Mismatched annotations are compile errors.

This explicitness ensures that reading a function call always reveals the
ownership impact on each argument, without consulting the function signature.

## 6.5 Call Semantics [Implemented]

### 6.5.1 Shared Borrow (Default)

When a function takes a shared parameter, the argument is borrowed immutably.
Multiple shared borrows of the same variable may coexist:

```lain
func length(s u8[:0]) int { return s.len }

var msg = "hello"
var a = length(msg)       // shared borrow
var b = length(msg)       // another shared borrow — OK
```

### 6.5.2 Mutable Borrow (`var`)

When a function takes a `var` parameter, the argument is borrowed exclusively.
No other borrow (shared or mutable) of the same variable shall be active:

```lain
func increment(var d Data) {
    d.value = d.value + 1
}

var data = Data(0)
increment(var data)       // exclusive mutable borrow
```

### 6.5.3 Ownership Transfer (`mov`)

When a function takes a `mov` parameter, the argument's ownership is
transferred. The source variable is invalidated:

```lain
func close_file(mov {handle} File) {
    fclose(handle)
}

var f = open_file("data.txt", "r")
close_file(mov f)         // f is consumed
// f cannot be used here — [E001]
```

## 6.6 Universal Function Call Syntax (UFCS) [Implemented]

Any function call `f(x, args...)` can be written as `x.f(args...)`:

```lain
func is_even(n int) bool {
    return n % 2 == 0
}

var x = 10
var even = x.is_even()    // equivalent to: is_even(x)
```

UFCS applies to all types and enables fluent, chainable APIs without
object-oriented programming.

### 6.6.1 UFCS Resolution

When the compiler encounters `x.f(args...)`:

1. First, it checks if `f` is a field of `x`'s type. If so, this is a
   member access, not UFCS.
2. If `f` is not a field, the compiler looks up a function named `f` whose
   first parameter matches the type of `x`.
3. If found, the call is desugared to `f(x, args...)`, with the appropriate
   ownership mode inferred from the function's first parameter.

### 6.6.2 UFCS and Ownership

When a UFCS call desugars to a function with a `var` first parameter,
the object is automatically mutably borrowed:

```lain
func push(var v Vec, item int) { ... }

var v = Vec(...)
v.push(42)                // desugars to: push(var v, 42)
```

## 6.7 Forward Declarations [Implemented]

See §3.9. Functions and procedures can be referenced before their
declaration point.

## 6.8 Termination Analysis [Implemented]

For `func`, the compiler statically verifies termination:

1. No `while` loops — prevents unbounded iteration.
2. No recursion (direct or indirect) — prevents infinite recursion.
3. Only `for` loops with finite ranges — always terminates.

> **CONSTRAINT:** A `func` that contains `while`, direct recursion, or
> indirect recursion (calling a function that transitively calls back)
> shall be rejected.

For `proc`, no termination analysis is performed.

---

*This chapter is normative.*
