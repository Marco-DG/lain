# Lain Ownership & Syntax Design — Unified Specification

This document synthesizes all design decisions into a single coherent reference.
It supersedes any conflicting syntax in earlier spec chapters or old documents.

---

## 1. Core Principles

1. **Explicitness**: If it's not in the source code, it's not happening.
2. **One keyword per concept**: No aliases, no overloading.
3. **Simplicity**: The entire language should fit in a programmer's head.
4. **Safety**: Resource lifecycles enforced at compile time with zero runtime overhead.

---

## 2. The `var` Keyword

`var` has one meaning: **mutation is allowed here.** It is used in two contexts:

1. **Variable declaration**: `var x = 10` — this variable can be reassigned.
2. **Mutable borrow**: `f(var x)` / `func f(var x T)` — this function can
   modify the caller's value.

These are the same concept. A mutable borrow grants write access to a variable,
which is only possible if the variable itself was declared `var`. The keyword
`var` always means "this thing can be written to."

```lain
x = 10               // immutable binding
var y = 20            // mutable binding (can be reassigned)

y = 30                // OK: y is var
x = 5                 // ERROR: x is not var

// With type annotation:
x int = 10            // immutable with explicit type
var y int = 20        // mutable with explicit type

// Uninitialized (requires definite initialization):
var z int = undefined
z = 42                // must assign before use
```

**Rules**:
- `name = expr` — if `name` is NOT in scope, creates a new **immutable** binding
- `name = expr` — if `name` IS in scope and `var`, it's **reassignment**
- `name = expr` — if `name` IS in scope and immutable, it's an **ERROR**
- Same-scope shadowing is an error
- You can only `var`-borrow a variable that was declared `var`

---

## 3. Ownership Model

Every value in Lain has exactly one owner. Values can be **borrowed** (shared
or mutably) or **moved** (ownership transferred). The model has three modes:

| Mode     | Meaning                          | Keyword  | At call site     | In parameter      |
|:---------|:---------------------------------|:---------|:-----------------|:-------------------|
| Shared   | Read-only borrow (many allowed)  | *(none)* | `f(x)`           | `func f(x T)`      |
| Mutable  | Exclusive read-write borrow      | `var`    | `f(var x)`       | `func f(var x T)`  |
| Owned    | Ownership transfer (consumes)    | `mov`    | `f(mov x)`       | `func f(mov x T)`  |

**Key rule**: At any program point, for any variable, either:
- Zero or more shared borrows are active, OR
- Exactly one mutable borrow is active

Never both simultaneously.

### 3.1 When to Use Each Mode

- **Shared** (no keyword): Reading data without modifying it.
  Works on any type, linear or non-linear.
- **`var`** (mutable borrow): Modifying data without consuming it.
  The caller keeps ownership; the function gets temporary write access.
  Works on any type — this is how you mutate both `Counter` and `File`.
- **`mov`** (move): Transferring ownership permanently.
  The caller loses the value. Required for destroying linear resources.

---

## 4. Linear Types

### 4.1 What Makes a Type Linear

A type is **linear** when declared with the `own` keyword. Linear values must
be consumed **exactly once** — no more, no less. This is opt-in.

```lain
// Linear type — must be consumed exactly once
type File own {
    handle *FILE
}

// Non-linear type — freely copyable
type Point {
    x int
    y int
}
```

**Why opt-in?** Plain data types (`Point`, `Color`, `Vec2`) should be freely
copyable without ceremony. Only resource-wrapping types (`File`, `Socket`,
`DbConnection`) need linearity. The programmer marks these explicitly with `own`.

### 4.2 Transitivity

If a struct contains a field whose type is linear, the struct is **automatically
linear** — no annotation needed:

```lain
type File own {
    handle *FILE
}

type FileLogger {
    file File        // FileLogger is automatically linear
    prefix string
}
```

### 4.3 Consuming Linear Values

Linear values are consumed by:
- Passing them with `mov` to a function: `close(mov f)`
- Moving them into a new variable: `var g = mov f`
- Destructuring them in a `mov` parameter: `func close(mov {handle} File)`
- Returning them: `return f`

### 4.4 `var` on Linear Types

A linear variable declared `var` can be reassigned, but the **old value must
be consumed first**:

```lain
var f = open("test.txt", "w")
f.close()                        // consume old value
f = open("other.txt", "w")      // OK: reassignment after consumption
f.close()                        // consume new value
```

Without consuming first:
```lain
var f = open("test.txt", "w")
f = open("other.txt", "w")      // ERROR [E002]: old f not consumed
```

### 4.5 Mutating Linear Types Without Consuming

`var` borrows let you modify a linear value without consuming it. This is the
key to ergonomic linear APIs:

```lain
proc write(var f File, s string) {
    fputs(s.data, f.handle)      // modifies f's state, doesn't consume
}

var f = open("test.txt", "w")
write(var f, "hello")            // var borrow: f is modified, not consumed
write(var f, "world")            // f is still alive
close(mov f)                     // now f is consumed
```

### 4.6 Violations

| Error    | Condition                                           |
|:---------|:----------------------------------------------------|
| `[E001]` | Use after move                                      |
| `[E002]` | Linear variable not consumed before scope exit      |
| `[E003]` | Linear variable consumed twice (double move)        |
| `[E006]` | Move inside a loop (would consume multiple times)   |
| `[E007]` | Consumed in one branch but not another              |

---

## 5. Functions and Procedures

### 5.1 Two Kinds of Callable

| Keyword | Name      | Pure | Loops | Recursion | Side Effects |
|:--------|:----------|:-----|:------|:----------|:-------------|
| `func`  | Function  | Yes  | No    | No        | No           |
| `proc`  | Procedure | No   | Yes   | Yes       | Yes          |

- `func` guarantees termination and has no side effects.
- `proc` has no restrictions. Use for I/O, loops, recursion, mutable globals.
- `func` cannot call `proc`. `proc` can call `func`.
- There is NO `fun` alias. The only keywords are `func` and `proc`.

### 5.2 Parameter Syntax

```lain
// Shared (read-only borrow)
func length(s string) int { ... }

// Mutable (exclusive borrow)
func increment(var counter Counter) { ... }

// Owned (consumes)
func close(mov f File) { ... }

// Destructuring an owned parameter
func close(mov {handle} File) {
    fclose(mov handle)
}
```

### 5.3 Return Types

Returns are **always owned**. The caller receives full ownership of the
returned value. There is no return-by-reference.

```lain
func open(path string) File { ... }     // returns owned File
func add(a int, b int) int { ... }      // returns owned int
func wrap(mov f File) Logger { ... }    // consumes f, returns owned Logger
```

Return constraints (VRA) are still supported:
```lain
func abs(x int) int >= 0 { ... }
```

---

## 6. UFCS (Uniform Function Call Syntax)

Any function can be called with method syntax. The first parameter becomes
the receiver:

```lain
// These are equivalent:
length(s)           // regular call
s.length()          // UFCS call

increment(var c)    // regular call
c.increment()       // UFCS call — mode inferred from signature

close(mov f)        // regular call
f.close()           // UFCS call — mode inferred from signature
```

### 6.1 Mode Inference

With UFCS, the compiler infers the ownership mode of the receiver from the
function's first parameter declaration. **No annotation needed at call site.**

| Definition                          | UFCS call         | Desugars to            |
|:------------------------------------|:------------------|:-----------------------|
| `func read(f File) string`          | `f.read()`        | `read(f)`              |
| `proc write(var f File, s string)`  | `f.write("hi")`   | `write(var f, "hi")`   |
| `proc close(mov f File)`            | `f.close()`       | `close(mov f)`         |

### 6.2 The File API Pattern

This is the canonical way to work with linear resources:

```lain
type File own {
    handle *FILE
}

proc open(path string, mode string) File { ... }
proc write(var f File, s string) { ... }
proc close(mov f File) { ... }

// Usage:
proc main() int {
    var f = open("test.txt", "w")
    f.write("hello world")     // var borrow, f survives
    f.write("another line")    // var borrow, f survives
    f.close()                  // mov, f consumed
    return 0
}
```

**Design principle**: Transforming operations (write, append, modify) use
`var` borrows. Destroying operations (close, free, drop) use `mov`.

---

## 7. Borrowing Rules

### 7.1 Temporary vs Persistent Borrows

**Temporary borrows** are created by function call arguments and expire at
end-of-statement:

```lain
read(data)           // temporary shared borrow — expires after this line
write(var data)      // temporary var borrow — expires after this line
// data is fully accessible here
```

Since there is no return-by-reference, there are no persistent borrows from
function returns. Borrows are always temporary (statement-scoped).

### 7.2 Two-Phase Borrows

When evaluating function arguments, mutable borrows start in a RESERVED
phase that permits shared reads of the same owner:

```lain
type Vec { data int, cap int }
proc push_n(var v Vec, n int) {
    v.data = v.data + n
}

var v = Vec(0, 10)
v.push_n(v.cap)      // OK: v.cap read during RESERVED phase of v's var borrow
```

### 7.3 Non-Lexical Lifetimes (NLL)

Borrows expire at their **last use**, not at the end of the enclosing scope:

```lain
var data = make_data()
result = read(data)       // shared borrow starts and expires (last use)
modify(var data)          // OK: borrow already expired
```

### 7.4 Non-Consuming Match

The `case &expr` syntax allows pattern matching without consuming linear values:

```lain
case &value {
    Pattern1(x): use(x)   // value is borrowed, not consumed
    Pattern2(y): use(y)
}
// value is still alive
```

### 7.5 Per-Field Borrowing

Different fields of the same struct can be borrowed independently:

```lain
type Pair { a int, b int }
func use_a(var p Pair) { p.a = p.a + 1 }

var pair = Pair(1, 2)
// Borrowing pair.a does not block access to pair.b
```

---

## 8. Complete Keyword Table (Ownership-Related)

| Keyword     | Meaning                                     | Example                         |
|:------------|:--------------------------------------------|:--------------------------------|
| `var`       | Mutation allowed (declaration or borrow)    | `var x = 10`, `f(var x)`       |
| `mov`       | Ownership transfer (move)                   | `f(mov x)`, `func f(mov x T)`  |
| `own`       | Linear type declaration                     | `type File own { ... }`         |
| `func`      | Pure function (no side effects)             | `func add(a int, b int) int`    |
| `proc`      | Procedure (side effects allowed)            | `proc main() int`               |
| `undefined` | Uninitialized variable marker               | `var x int = undefined`         |

---

## 9. What Changed (Migration from Previous Spec)

| Before (old spec)                    | After (this document)              | Reason                           |
|:-------------------------------------|:-----------------------------------|:---------------------------------|
| `fun` alias for `func`              | Removed                            | One keyword per concept          |
| `type File { mov handle *FILE }`    | `type File own { handle *FILE }`   | Explicit linear type declaration |
| `return var expr` for ref return    | Removed                            | Returns are always owned         |
| `func f() var T` return-by-ref      | Removed                            | Simplicity, no persistent borrows|

**What stayed the same:**
- `var` for mutable variables AND mutable borrows (same concept)
- `mov` for ownership transfer
- `func`/`proc` distinction
- Implicit shared borrows (no keyword)

---

## 10. Canonical Examples

### 10.1 Hello World

```lain
import std.io

proc main() int {
    println("Hello, world!")
    return 0
}
```

### 10.2 Pure Function

```lain
func max(a int, b int) int {
    if a > b { return a }
    return b
}
```

### 10.3 Linear Resource (File)

```lain
import std.fs

proc main() int {
    var f = open("output.txt", "w")
    f.write("Hello from Lain!")
    f.close()
    return 0
}
```

### 10.4 Generics

```lain
func identity(comptime T type, x T) T {
    return x
}

proc main() int {
    result = identity(int, 42)
    return result
}
```

### 10.5 ADT with Pattern Matching

```lain
type Shape {
    Circle { radius int }
    Rectangle { width int, height int }
}

func area(s Shape) int {
    case s {
        Circle(r): return r * r * 3
        Rectangle(w, h): return w * h
    }
}
```

### 10.6 Option Type

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}

func find(arr int[10], target int) Option(int) {
    for i in 0..10 {
        if arr[i] == target {
            return Option(int).Some(i)
        }
    }
    return Option(int).None
}
```

### 10.7 Ownership in Action

```lain
type Resource own {
    id int
}

func create(id int) Resource {
    return Resource(id)
}

func transform(mov r Resource) Resource {
    new_id = r.id + 1
    return Resource(new_id)
}

func destroy(mov {id} Resource) {
    // id consumed by destructuring
}

proc main() int {
    var r = create(1)
    r = transform(mov r)    // old r consumed, rebound to new value
    destroy(mov r)           // r consumed permanently
    return 0
}
```

### 10.8 Mutable Borrows (Non-Linear Type)

```lain
type Counter {
    value int
}

func increment(var c Counter, amount int) {
    c.value = c.value + amount
}

func get(c Counter) int {
    return c.value
}

proc main() int {
    var c = Counter(0)
    c.increment(5)           // UFCS: increment(var c, 5)
    c.increment(3)           // UFCS: increment(var c, 3)
    return c.get()           // UFCS: get(c) -> 8
}
```

### 10.9 Mutable Borrows (Linear Type)

```lain
type File own {
    handle *FILE
}

proc open(path string, mode string) File {
    return File(fopen(path.data, mode.data))
}

proc write(var f File, s string) {
    fputs(s.data, f.handle)
}

proc close(mov {handle} File) {
    fclose(mov handle)
}

proc main() int {
    var f = open("log.txt", "w")
    f.write("Starting up...")       // var borrow: f modified, not consumed
    f.write("All systems go.")      // var borrow: f modified, not consumed
    f.close()                       // mov: f consumed, cannot use after this
    return 0
}
```

---

## 11. Summary: How It All Fits Together

```
                    ┌─────────────────────────────┐
                    │        Type Declaration      │
                    │                              │
                    │  type Point { x int, y int } │  ← non-linear (copyable)
                    │  type File own { handle *FILE}│  ← linear (must consume)
                    └─────────────────────────────┘
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
              Non-linear types            Linear types (own)
              (freely copyable)           (exactly-once use)
                    │                           │
                    ├── shared borrow: f(x)     ├── shared borrow: f(x)
                    ├── var borrow: f(var x)    ├── var borrow: f(var x)
                    └── copy (implicit)         └── mov: f(mov x)
                                                     └── consumes the value

              UFCS: x.method() infers the mode from method's first parameter
```

---

*This document is the canonical reference for Lain's ownership and syntax design.
All specification chapters should be updated to match.*
