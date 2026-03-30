# Chapter 5 — Statements

## 5.1 Statement Termination [Implemented]

Statements are terminated by newlines or semicolons. Multiple statements
may appear on the same line if separated by semicolons:

```lain
var x = 1; var y = 2      // two statements on one line
var z = 3                  // newline terminates
```

## 5.2 Variable Declaration Statement [Implemented]

See §3.2 for the full specification of variable declarations.

```lain
var x = 42                 // mutable binding
var y int = 0              // mutable with explicit type
x = 10                     // immutable binding (if x not in scope)
```

## 5.3 Assignment Statement [Implemented]

An assignment modifies the value of a mutable variable:

```lain
x = 42                     // simple assignment
p.x = 10                   // field assignment
arr[i] = 99                // index assignment
```

> **CONSTRAINT:** The left-hand side shall be a mutable lvalue. Assigning
> to an immutable variable produces diagnostic `[E009]`.

> **CONSTRAINT:** The type of the right-hand side shall match the type of
> the left-hand side.

### 5.3.1 Compound Assignment

See §4.7.1.

## 5.4 Expression Statement [Implemented]

An expression followed by a statement terminator is an expression statement.
The value of the expression is discarded:

```lain
do_something()             // function call as statement
x + 1                      // value discarded (usually a mistake)
```

## 5.5 If / Elif / Else [Implemented]

```lain
if condition1 {
    // then branch
} elif condition2 {
    // elif branch
} else {
    // else branch
}
```

### 5.5.1 Syntax

```
if_stmt = "if" expr block { "elif" expr block } [ "else" block ] ;
block   = "{" { statement } "}" ;
```

### 5.5.2 Semantics

1. The condition expression shall have type `bool` or be an integer expression
   (integers are implicitly compared to zero in condition position).
2. The then-branch is executed if the condition is true.
3. `elif` branches are tested sequentially if previous conditions were false.
4. The `else` branch executes if all conditions were false.
5. Braces `{ }` are always required around branches.

### 5.5.3 Range Refinement

Inside a branch, the compiler refines the known value ranges of variables
used in the condition (see §8):

```lain
if x < 10 {
    // compiler knows: x in [INT_MIN, 9]
} else {
    // compiler knows: x in [10, INT_MAX]
}
```

### 5.5.4 In-Guard Refinement [Implemented]

When the condition contains an `in` expression (see §4.19), the compiler
creates an **in-guard** that permits the guarded array access inside the
then-branch without further bounds checking:

```lain
if pos in data {
    return data[pos] as int    // safe: in-guarded
}
```

In-guards are scoped to the then-branch only — they do not extend to
`elif` or `else` branches (the negation of `in` proves nothing useful).

## 5.6 Loops [Implemented]

### 5.6.1 For Loop (Range-Based)

```lain
for i in 0..n {
    // i ranges from 0 to n-1 (exclusive end)
}
```

**Syntax:**
```
for_stmt = "for" IDENTIFIER ["," IDENTIFIER] "in" expr ".." expr block ;
```

**Two-variable form:**
```lain
for i, val in 0..10 {
    // i = index, val = value (both equal for integer ranges)
}
```

**Loop variable properties:**
- The loop variable is immutable within the loop body.
- Its value range is statically known: `i in [start, end-1]`.
- This enables safe array indexing without runtime checks.

**Availability:** `for` is allowed in both `func` and `proc`.

### 5.6.2 While Loop

```lain
while condition {
    // loop body (proc only)
}

while condition decreasing measure {
    // loop body (allowed in func — bounded while)
}
```

**Syntax:**
```
while_stmt = "while" expr [ "decreasing" expr ] block ;
```

The condition is evaluated before each iteration. The body executes as long
as the condition is true.

When the condition contains an `in` expression (see §4.19), the compiler
creates an in-guard for the loop body, permitting the guarded array access:

```lain
while i in data decreasing data.len - i {
    data[i]          // safe: in-guarded by the while condition
    i += 1
}
```

> **CONSTRAINT:** Unbounded `while` loops (without a termination measure) are
> only allowed in `proc` (see §6.1). Using an unbounded `while` inside a
> `func` produces diagnostic `[E011]`.

**Infinite loop pattern:**
```lain
while 1 {
    if done { break }
}
```

### 5.6.2.1 Bounded While — Termination Measure [Implemented]

A `while` loop may carry an optional **termination measure** via the
`decreasing` keyword. The measure is an integer expression that the compiler
statically verifies:

1. **Non-negative**: The measure is ≥ 0 when the loop condition is true.
2. **Strictly decreasing**: The loop body contains assignments that strictly
   decrease the measure on each iteration.

When a termination measure is present, the `while` loop is allowed inside
`func` (pure functions), because the compiler can guarantee termination.

```lain
func count_up(n int) int {
    var i = 0
    while i < n decreasing n - i {  // measure: n - i
        i += 1                   // i increases → n - i decreases
    }
    return i
}
```

**How the compiler verifies the measure:**

The compiler performs two checks:

1. **Non-negativity**: Pattern-matches the while condition against the measure
   expression. For example, condition `a < b` with measure `b - a` implies
   `b - a > 0` when the condition is true.

2. **Strict decrease**: Extracts variables from the measure and their polarity
   (positive or negative). For measure `b - a`, variable `a` has negative
   polarity and `b` has positive polarity. Then it scans all assignments in
   the loop body and verifies that every assignment to a measure variable
   moves the measure in the decreasing direction. For example, `a += 1`
   increases `a` (negative polarity), so the measure `b - a` decreases.

**Supported patterns:**

| Condition | Measure | Why it works |
|:----------|:--------|:-------------|
| `i < n` | `n - i` | `i` increases → measure decreases |
| `pos < size` | `size - pos` | `pos` increases → measure decreases |
| `n > 0` | `n` | `n` decreases directly |
| `a < b` | `b - a` | Either `a` increases or `b` decreases |
| `i in arr` | `arr.len - i` | `in` implies `i < arr.len` (§4.19) |

**Struct fields are supported**: The verification works with member expressions
like `l.pos` and `l.size`, not just local variables.

**Error diagnostics:**

| Code | Meaning |
|:-----|:--------|
| `[E080]` | Cannot verify measure is non-negative when condition holds |
| `[E081]` | Cannot extract variables from measure expression |
| `[E082]` | Cannot verify measure strictly decreases each iteration |

> **RATIONALE:** This feature exists because finite state machines (lexers,
> parsers, protocol handlers) are provably terminating on finite input, yet
> the previous `func` restriction (`while` banned) forced them to use `proc`.
> The `decreasing` keyword bridges this gap: the programmer states *why* the
> loop terminates, and the compiler verifies it.

### 5.6.3 Break

`break` exits the innermost enclosing loop:

```lain
while true {
    if condition { break }   // exit the while loop
}
```

### 5.6.4 Continue

`continue` skips to the next iteration of the innermost enclosing loop:

```lain
for i in 0..10 {
    if i == 5 { continue }   // skip i = 5
    use(i)
}
```

> **CONSTRAINT:** `break` and `continue` shall only appear inside a loop body.

## 5.7 Case Statement (Pattern Matching) [Implemented]

The `case` statement matches a value against a series of patterns:

```lain
case value {
    pattern1: action1
    pattern2: action2
    else: default_action
}
```

### 5.7.1 Syntax

```
case_stmt = "case" ["&"] expr "{" { case_arm } "}" ;
case_arm  = pattern { "," pattern } ":" (expr | block) ;
pattern   = IDENTIFIER [ "(" pattern_list ")" ]
          | literal
          | range
          | "else" ;
```

### 5.7.2 Supported Pattern Types

**Integer patterns:**
```lain
case x {
    1: action_one()
    2: action_two()
    else: default()        // required for integers
}
```

**Character patterns:**
```lain
case ch {
    'a'..'z': lowercase()
    'A'..'Z': uppercase()
    else: other()
}
```

**Enum patterns:**
```lain
case color {
    Red: handle_red()
    Green: handle_green()
    Blue: handle_blue()
}
```

**ADT patterns with destructuring:**
```lain
case shape {
    Circle(r):       area = r * r * 314 / 100
    Rectangle(w, h): area = w * h
    Point:           area = 0
}
```

**Multiple patterns (OR):**
```lain
case x {
    1, 2, 3: small()
    4, 5, 6: medium()
    else: large()
}
```

**Range patterns:**
```lain
case x {
    1..10: small()         // matches 1 through 10 (inclusive)
    11..100: medium()
    else: large()
}
```

### 5.7.3 Case Arms

Each case arm may be:
- A single expression: `Red: return 1`
- A block: `Red: { do_stuff(); return 1 }`

### 5.7.4 Exhaustiveness Checking

> **CONSTRAINT:** `case` statements shall be **exhaustive**. The compiler
> verifies:
>
> - **Enums**: All variants shall be covered, OR an `else` arm shall be present.
> - **ADTs**: All variants shall be covered, OR an `else` arm shall be present.
> - **Integers**: An `else` arm is always required.
> - **Characters**: An `else` arm is always required.
>
> A non-exhaustive `case` produces diagnostic `[E014]`.

### 5.7.5 Non-Consuming Match (`case &expr`) [Implemented]

By default, `case expr` may consume the scrutinee (for linear types, this
transfers ownership). To inspect a value without consuming it, prefix the
scrutinee with `&`:

```lain
var x = 42
var result = 0
case &x {
    42: result = 1
    else: result = 0
}
// x is still usable
return x + result          // returns 43
```

The `&` prefix registers a shared borrow on the scrutinee for the duration
of the match body. This prevents mutation of the scrutinee inside the
match arms:

```lain
case &x {
    42: x = 99             // ERROR [E004]: cannot mutate 'x' because
                           //               it is borrowed
    else: x = 0
}
```

For non-linear types, `case &` is semantically identical to `case` but
enforces immutability of the scrutinee within the match body.

For linear types, `case &` prevents consumption of the value, allowing
it to be used after the match.

### 5.7.6 Case Expression

`case` can be used as an expression (see §4.15):

```lain
var name = case color {
    Red: "red"
    Green: "green"
    Blue: "blue"
}
```

> **CONSTRAINT:** All branches of a case expression shall yield the same type.

## 5.8 Defer Statement [Implemented]

The `defer` statement schedules a block for execution at scope exit:

```lain
proc process_file() {
    var f = open_file("data.txt", "r")
    defer {
        close_file(mov f)
    }
    // ... use f ...
    // f is automatically closed when the function returns
}
```

### 5.8.1 Syntax

```
defer_stmt = "defer" block ;
```

### 5.8.2 Semantics

1. Deferred blocks execute in **LIFO order** (last `defer` first).
2. They execute on all exit paths: end of block, `return`, `break`, `continue`.
3. Multiple `defer` statements accumulate within a scope and execute in
   reverse order of declaration.

### 5.8.3 Restrictions

> **CONSTRAINT:** `defer` blocks shall not contain `return`, `break`, or
> `continue` statements that would transfer control out of the deferred block.

## 5.9 Return Statement [Implemented]

```lain
return expr               // return a value
return mov expr           // return with ownership transfer
return var expr           // return a mutable reference
return                    // return void (procedures only)
```

### 5.9.1 Return Ownership Modes

| Syntax | Semantics |
|:-------|:----------|
| `return expr` | Return by value (copy) |
| `return mov expr` | Transfer ownership to caller |
| `return var expr` | Return a mutable reference |

> **CONSTRAINT:** `return var` shall only return references to data that
> outlives the function — typically fields of `var` parameters. Returning
> a `var` reference to a local variable produces diagnostic `[E010]`
> (dangling reference).

## 5.10 Unsafe Block [Implemented]

```lain
unsafe {
    // pointer operations allowed here
}
```

See §12 for the complete specification of unsafe code.

---

*This chapter is normative.*
