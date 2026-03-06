# Chapter 7 — Ownership & Borrowing

This chapter specifies Lain's ownership and borrowing system — the foundation
of its memory safety guarantees. All rules in this chapter are enforced at
compile time with zero runtime overhead.

## 7.1 Ownership Modes [Implemented]

Every parameter, variable, and struct field has one of three ownership modes:

| Mode | Parameter Syntax | Call-Site Syntax | Semantics |
|:-----|:-----------------|:-----------------|:----------|
| **Shared** | `f(x T)` | `f(x)` | Immutable borrow. Multiple simultaneous shared borrows allowed. |
| **Mutable** | `f(var x T)` | `f(var x)` | Exclusive read-write borrow. Only one at a time. |
| **Owned** | `f(mov x T)` | `f(mov x)` | Ownership transfer. Source variable invalidated. |

### 7.1.1 Internal Representation

In the compiler's type system, ownership mode is a property of the type:

```
MODE_SHARED   — default, read-only access
MODE_MUTABLE  — exclusive read-write access
MODE_OWNED    — ownership transfer (linear)
```

## 7.2 Move Semantics [Implemented]

### 7.2.1 The `mov` Operator

The `mov` operator transfers ownership of a value from the source to the
destination. After a move, the source variable is **invalidated** — any
subsequent use is a compile error.

```lain
var a = Resource(1)
var b = mov a              // ownership transferred from a to b
// a is now invalid
```

### 7.2.2 Move at Call Site

```lain
consume(mov resource)
// resource is now invalid — any subsequent use is [E001]
```

### 7.2.3 Move in Return

```lain
func wrap(mov r Resource) Container {
    return Container(mov r)  // transfer ownership into return value
}
```

### 7.2.4 Use After Move

> **CONSTRAINT:** Using a variable after it has been moved shall produce
> diagnostic `[E001]` (use after move).

```lain
var f = open_file("data.txt", "r")
close_file(mov f)
// ERROR [E001]: cannot use 'f': value was moved
read_file(f)
```

### 7.2.5 Double Move

> **CONSTRAINT:** Moving a variable that has already been moved shall
> produce diagnostic `[E003]` (double move).

```lain
var f = open_file("data.txt", "r")
close_file(mov f)
close_file(mov f)          // ERROR [E003]: 'f' already moved
```

## 7.3 The Borrow Checker [Implemented]

### 7.3.1 Core Invariant

At every program point, for every variable `V`:

> **Either** zero or more shared borrows of `V` are active, **or** exactly
> one mutable borrow of `V` is active — **never both simultaneously**.

This is the Read-Write Lock invariant. All violations are compile errors.

### 7.3.2 Borrow Conflict Rules

| Active Borrow | Attempted Operation | Result |
|:--------------|:--------------------|:-------|
| None | Shared borrow | Allowed |
| None | Mutable borrow | Allowed |
| None | Move | Allowed |
| Shared borrow(s) | Shared borrow | Allowed |
| Shared borrow(s) | Mutable borrow | **Error** `[E004]` |
| Shared borrow(s) | Move | **Error** `[E005]` |
| Mutable borrow | Shared borrow | **Error** `[E004]` |
| Mutable borrow | Mutable borrow | **Error** `[E004]` |
| Mutable borrow | Move | **Error** `[E005]` |

### 7.3.3 Borrow Registration

A borrow is registered when a variable is passed as an argument to a
function. The borrow is associated with:

- **The variable being borrowed** (the owner)
- **The borrow mode** (shared or mutable)
- **A binding identifier** (the parameter name or temporary)
- **A region** (scope in which the borrow is valid)

### 7.3.4 Temporary vs Persistent Borrows

**Temporary borrows** are created by function call arguments and expire
at the end of the statement:

```lain
read(data)                 // temporary shared borrow — expires after statement
// data is fully accessible here
```

**Persistent borrows** are created by return-by-reference functions and
persist until the returned reference's last use:

```lain
var ref = get_ref(var data)  // persistent mutable borrow of data
// data is inaccessible while ref is alive
use(var ref)                 // last use of ref — persistent borrow expires
// data is accessible again
```

## 7.4 Non-Lexical Lifetimes (NLL) [Implemented]

Borrows expire at their **last use**, not at the end of the enclosing scope.
This is computed by a use-analysis pre-pass that identifies the last program
point where each variable is read.

### 7.4.1 NLL in Practice

```lain
var data = Data(42)
var ref = get_ref(var data)   // mutable borrow of data starts
consume_ref(var ref)          // last use of ref
// mutable borrow of data expires here (NLL)
var x = read_data(data)       // OK: borrow has expired
```

Without NLL, the borrow of `data` would persist until `ref` goes out of
scope, making the `read_data` call illegal.

### 7.4.2 NLL in Control Flow

Temporary borrows within `if`, `while`, `for`, and `case` conditions do
not span the entire block. They are released immediately after the
condition is evaluated:

```lain
var data = Buffer(0)
// OK: shared borrow of data in condition expires after evaluation
while read(data) < 10 {
    mutate(var data)       // mutable borrow OK — condition borrow expired
}
```

## 7.5 Two-Phase Borrows [Implemented]

### 7.5.1 The Problem

When calling a method via UFCS (e.g., `x.method(x.field)`), the desugaring
produces `method(var x, x.field)`. Without special handling, the mutable
borrow of `x` for the first parameter would block the shared read of
`x.field` for the second parameter.

### 7.5.2 The Solution

During argument evaluation, mutable borrows are registered in a
**RESERVED** phase that permits shared reads of the same owner:

| Phase | Allows | Blocks |
|:------|:-------|:-------|
| RESERVED | Shared reads of the owner | Mutable writes, moves |
| ACTIVE | Nothing (fully exclusive) | All other access |

### 7.5.3 Lifecycle

1. When a `var` argument is evaluated, the mutable borrow is registered
   as `BORROW_RESERVED`.
2. While evaluating subsequent arguments, shared reads of the same owner
   are permitted (they pass the conflict check).
3. After all arguments are evaluated, all RESERVED borrows are promoted
   to `BORROW_ACTIVE`.
4. At end-of-statement, temporary borrows are cleared.

### 7.5.4 Example

```lain
type Vec { data int, cap int }
proc push_n(var v Vec, n int) {
    v.data = v.data + n
}

var v = Vec(0, 10)
v.push_n(v.cap)           // OK: v.cap is a shared read during RESERVED phase
```

### 7.5.5 Safety

Two-phase borrows do **not** permit:
- Moves of the owner during RESERVED phase
- Mutable writes to the owner during RESERVED phase
- Multiple mutable borrows of the same owner

## 7.6 Linear Types [Implemented]

### 7.6.1 Definition

A type is **linear** if:
- It has a `mov` annotation, OR
- It contains a field with a `mov` annotation, OR
- It contains a field whose type is linear (transitivity)

Linear values must be consumed **exactly once**. Forgetting to consume a
linear value, or consuming it twice, is a compile error.

### 7.6.2 Unconsumed Linear Values

> **CONSTRAINT:** A linear variable that goes out of scope without being
> consumed shall produce diagnostic `[E002]`.

```lain
proc leak() {
    var f = open_file("data.txt", "r")
    // ERROR [E002]: linear variable 'f' was not consumed before scope exit
}
```

### 7.6.3 Consuming Linear Values

Linear values are consumed by:
- Passing them with `mov` to a function
- Moving them into a new variable with `mov`
- Destructuring them in a `mov` parameter
- Returning them with `return mov`

### 7.6.4 Linear Struct Fields

Fields annotated with `mov` make the containing struct linear:

```lain
type File {
    mov handle *FILE       // File is linear
}

type Wrapper {
    f File                 // Wrapper is also linear (transitively)
}
```

> **CONSTRAINT:** A struct with linear fields shall not be copied. Only
> move semantics are allowed.

## 7.7 Branch Consistency [Implemented]

### 7.7.1 Consistent Consumption

Linear variables must be consumed consistently across all branches of
an `if`/`elif`/`else` chain:

```lain
var resource = create_resource()
if condition {
    consume(mov resource)  // consumed in then branch
} else {
    consume(mov resource)  // must also be consumed in else branch
}
```

> **CONSTRAINT:** If a linear variable is consumed in one branch but not
> another, the compiler shall produce diagnostic `[E007]`.

### 7.7.2 Move State After Branches

After an `if`/`else` block, a variable's move state is the intersection
of its states across all branches:
- If consumed in ALL branches: variable is consumed after the block.
- If consumed in NO branches: variable is alive after the block.
- If consumed in SOME branches: compile error `[E007]`.

## 7.8 Move in Loops [Implemented]

> **CONSTRAINT:** Moving a variable inside a loop body (where the loop
> may execute multiple iterations) shall produce diagnostic `[E006]`.

```lain
var resource = create_resource()
while condition {
    consume(mov resource)  // ERROR [E006]: cannot move inside a loop
}
```

This prevents double-consumption when the loop executes more than once.

## 7.9 Non-Consuming Match [Implemented]

The `case &expr` syntax (see §5.7.5) allows pattern matching without
consuming the scrutinee. A persistent shared borrow is registered for
the duration of the match body:

```lain
case &value {
    Pattern1(x): use(x)   // value is borrowed, not consumed
    Pattern2(y): use(y)
}
// value is still available
```

The persistent borrow prevents mutation of the scrutinee within the
match body. The borrow is removed when the match block completes.

## 7.10 Borrow Checking Across Scopes [Implemented]

### 7.10.1 Block Scope Exit

When a block scope exits, all borrows registered within that scope are
released:

```lain
var data = Data(42)
{
    var ref = get_ref(var data)  // persistent borrow starts
    use(var ref)                 // borrow active
}                               // block exits — borrow released
read(data)                      // OK: borrow is gone
```

### 7.10.2 Dangling Reference Prevention

> **CONSTRAINT:** A `return var` statement shall not return a reference to
> a local variable. The referenced data must outlive the function.
> Violation produces diagnostic `[E010]`.

```lain
func bad_ref() var int {
    var x = 42
    return var x           // ERROR [E010]: x does not outlive the function
}

func ok_ref(var ctx Context) var int {
    return var ctx.counter  // OK: ctx outlives the function
}
```

### 7.10.3 Owner Reassignment

> **CONSTRAINT:** A variable that has an active borrow shall not be
> reassigned. Violation produces diagnostic `[E004]`.

```lain
var data = Data(42)
var ref = get_ref(var data)   // data is mutably borrowed
data = Data(99)               // ERROR [E004]: data is borrowed
use(var ref)                  // last use — borrow expires
data = Data(99)               // OK now
```

## 7.11 Per-Field Borrowing [Implemented]

The borrow checker tracks borrows at the field level. Borrowing different
fields of the same struct does not conflict:

```lain
type Pair { a int, b int }

func use_a(var p Pair) int { return p.a }
func use_b(var p Pair) int { return p.b }

var pair = Pair(1, 2)
// Both borrows are on different fields — no conflict
```

> Note: Per-field borrowing is tracked at the first level only. Nested field
> tracking (e.g., `p.a.x` vs `p.a.y`) is not currently implemented.

## 7.12 Summary of Diagnostics

| Code | Condition |
|:-----|:----------|
| `[E001]` | Use after move |
| `[E002]` | Unconsumed linear value at scope exit |
| `[E003]` | Double move |
| `[E004]` | Borrow conflict (mut+shared, mut+mut, mutation during borrow) |
| `[E005]` | Move of borrowed value |
| `[E006]` | Move inside a loop |
| `[E007]` | Branch inconsistency (consumed in one branch but not another) |
| `[E008]` | Linear struct field error |

---

*This chapter is normative. The ownership and borrowing rules specified herein
are the foundation of Lain's memory safety guarantees. A conforming implementation
shall enforce every constraint in this chapter.*
